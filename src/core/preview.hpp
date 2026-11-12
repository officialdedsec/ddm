// Play while downloading: DDM serves a download to a player on this PC only (127.0.0.1, a secret address that changes
// every time DDM starts). The player asks for any part of the file; what has arrived is sent at once, and for what
// hasn't, the download starts a connection right there. Works for videos and music kept as one file.
#pragma once
#include "model.hpp"
#include <string>

namespace ddm {
namespace preview {

bool canPlay(const Task& t);                  // a video / music file that can be played now (finished or downloading)
std::string urlFor(const std::string& taskId);   // "" = can't (the local server couldn't start)
void stop();

} // namespace preview
} // namespace ddm
