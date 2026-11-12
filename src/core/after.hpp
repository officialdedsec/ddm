// What happens after a download finishes, in the background: the antivirus scan (and the other automatic checks).
#pragma once
#include <functional>
#include <string>

namespace ddm {
namespace after {
void finished(const std::string& id);   // queue the checks for a download that just completed
void scanNow(const std::string& id);    // right-click → Scan for viruses
// title, text, id: something the user should know (a threat); called on a worker thread
extern std::function<void(const std::string&, const std::string&, const std::string&)> onAlert;
// title, text: a quiet note (no sound), e.g. "this video has no subtitles in English"
extern std::function<void(const std::string&, const std::string&)> onNote;
} // namespace after
} // namespace ddm
