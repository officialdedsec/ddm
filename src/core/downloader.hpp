// The multi-connection HTTP/FTP downloader (one running download).
#pragma once
#include "http.hpp"
#include "model.hpp"
#include <atomic>
#include <functional>
#include <mutex>

namespace ddm {

// Shared by all downloads: the "all downloads together" speed limit (bytes per second, 0 = none).
void setGlobalSpeedLimit(int64_t bytesPerSec);
// Everything DDM's HTTP engine has received (to tell DDM's traffic from other programs').
int64_t receivedBytes();

struct DownloadContext {
    Task* task = nullptr;          // owned by the engine; guarded by mu
    std::mutex* mu = nullptr;
    std::atomic<bool>* stop = nullptr;
    std::function<void()> changed; // progress or state text changed (throttled by the caller)

    // Services that address byte ranges in the URL and encrypt content (MEGA). All optional.
    int64_t knownTotal = -1;                                  // size known up front: every connection starts at once
    std::function<std::string(int64_t from)> rangeUrl;       // URL for bytes from..end (no Range header)
    std::function<void(int64_t offset, char* data, size_t len)> transform;   // decrypt in place before writing
    std::function<int()> renew;                               // address refused (403/404/410): 1 = got a new one, 2 = already renewed, 0 = no
    std::function<std::string(long status)> statusError;     // service-specific message for a failure status
    std::function<std::string()> urlNow;                      // the address to use for the next request (after renew())
    int64_t maxRequest = 0;                                   // >0: no single request asks for more than this (video sites that slow big requests)
};

enum class DownloadResult { Done, Stopped, Failed };

// Runs until the file is complete, stopped, or failed (task->error says why). Resumes from task->segments.
DownloadResult runHttpDownload(DownloadContext& ctx);
// Play while downloading: the player needs this byte of this download next. For about 20 seconds the next connection
// that becomes free starts there (the download goes on as before; nothing else changes).
void wantByte(const std::string& taskId, int64_t offset);

// Asks the server about a link without downloading it (size, name, resume support, final address).
bool probe(const RequestInfo& r, ResponseInfo& out, std::string* error = nullptr);

std::string friendlyError(long curlCode, long httpStatus);

} // namespace ddm
