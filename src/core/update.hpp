// "Is there a newer DDM?" — reads the signed update.json on dedseec.com (at most once a day, never installs by itself).
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace ddm {
namespace update {

struct Info {
    bool newer = false;
    std::string version, url, setup, setupSha, notes, error;   // setupSha: optional "sha256" of the installer
};

Info check(bool force);   // network; call from a worker thread
bool isNewer(const std::string& a, const std::string& b);
// Downloads the installer (https only) with progress; returns the local path or "" (error filled).
std::string downloadSetup(const std::string& url, std::function<void(int64_t, int64_t)> progress, std::string& error);

} // namespace update
} // namespace ddm
