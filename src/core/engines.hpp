// DDM's own working folder (data\engine) and the program that joins video and sound (ffmpeg.exe, installed with DDM
// next to DDM.exe). DDM fetches no outside programs: videos, torrents and File tools are all built in.
#pragma once
#include <string>

namespace ddm {
namespace engines {

std::string dir();                             // data\engine (kept files such as .torrent copies)
bool is64();
int windowsMajor();
void removeOld();                              // helper programs earlier versions fetched (removed: no longer used)
// The program that joins video and sound and makes MP3s: installed with DDM. "" = missing (reinstall DDM).
std::string joiner();
std::string joinerDir();

} // namespace engines
} // namespace ddm
