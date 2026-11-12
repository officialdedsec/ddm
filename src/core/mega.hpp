// MEGA (mega.nz) share links: files and folders, decrypted on the fly while DDM's 32-connection engine downloads.
#pragma once
#include "downloader.hpp"
#include <string>
#include <vector>

namespace ddm {

bool isMegaLink(const std::string& url);
bool isMegaFolderLink(const std::string& url);
std::string megaProblem(const std::string& url);            // why a MEGA link can't work, "" if fine

struct MegaEntry {
    std::string url;      // a link to this one file (folder files: .../folder/<h>#<key>/file/<node>)
    std::string name;
    std::string path;     // sub-folder inside the shared folder ("" = top)
    int64_t size = -1;
};

// One file: its name and size. Folder: every file in it. error gets a friendly message.
bool megaInfo(const std::string& url, MegaEntry& out, std::string& error);
bool megaFolder(const std::string& url, std::string& folderName, std::vector<MegaEntry>& files, std::string& error);

DownloadResult runMegaDownload(DownloadContext& ctx);

} // namespace ddm
