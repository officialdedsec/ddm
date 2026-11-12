// Runs a helper program without any window, reads what it prints line by line, and can stop it with everything it started.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace ddm {

struct RunResult {
    int exitCode = -1;
    std::string out, err;
    bool started = false, stopped = false;
};

// onLine gets every line of standard output (and error, prefixed "ERR: "). stop is polled; timeoutMs 0 = none.
RunResult runHidden(const std::string& exe, const std::vector<std::string>& args, std::function<void(const std::string&)> onLine = nullptr,
                    std::atomic<bool>* stop = nullptr, int timeoutMs = 0, const std::string& workDir = "");

std::string quoteArg(const std::string& a);

} // namespace ddm
