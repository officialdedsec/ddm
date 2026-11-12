// "Mark of the Web": the note browsers attach to every downloaded file (the file's Zone.Identifier stream) saying it
// came from the internet. Windows SmartScreen, "This file came from another computer", Office's Protected View and
// many security tools rely on it. Most download managers skip it; DDM adds it like a browser does.
#pragma once
#include <string>

namespace ddm {
namespace motw {

// Marks a file (or every file in a folder, for torrents) as downloaded from url. Credentials and "#…" parts of the
// address are never written. Does nothing for files from this PC or on drives without NTFS streams.
void mark(const std::string& path, const std::string& url, const std::string& referrer);
bool isMarked(const std::string& path);

} // namespace motw
} // namespace ddm
