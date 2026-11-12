// What kind of link is this (file, magnet, torrent, MEGA, video page, web page), and Google Drive file links.
#pragma once
#include <map>
#include <string>

namespace ddm {
namespace links {

enum class Type { DirectFile, Magnet, TorrentFile, Mega, Video, Web };

Type classify(const std::string& url);
std::string magnetName(const std::string& uri);

bool isDrive(const std::string& url);
// The direct download address of a Drive file (follows the "can't scan this large file" page). Network.
std::string resolveDrive(const std::string& url, const std::string& cookies);

} // namespace links
} // namespace ddm
