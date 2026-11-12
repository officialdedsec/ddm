// Phone remote: a page on your home network (Tools → Phone remote, off until you turn it on). Your phone opens it
// by scanning the QR code: the address holds a secret key, and nothing works without it. The page shows the
// downloads (progress, KB/s, time left) and can add a link, pause, resume, stop or remove downloads.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace ddm {
namespace remote {

struct Ask { std::string action, id, url; };   // (handed to DDM's window)

// What the phone asked for, done by DDM's window: action "add" (url), "pause" / "resume" / "stop" / "remove" (id),
// "pauseall" / "resumeall".
void setHandler(std::function<void(const std::string& action, const std::string& id, const std::string& url)> fn);
bool start(std::string& error);   // with Options' key and port (a key is made the first time)
void stop();
bool running();
std::string newKey();              // a new secret: phones with the old address can't get in any more
std::vector<std::string> addresses();   // "http://192.168.1.20:8473/?k=…" for each network of this PC

} // namespace remote
} // namespace ddm
