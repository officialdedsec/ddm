// Link & file check: DDM looks at where a file comes from and what it really is — things an antivirus never sees —
// before the file is on your PC, and again when it arrives.
//   The link:  a site registered days ago, a name that imitates a well-known brand (micros0ft-download.com), letters
//              from other alphabets, a bare IP address, a link shortener, risky top-level domains, no encryption for a
//              program, a site listed for spreading malware (URLhaus, updated in the background).
//   The file:  a program hidden behind a picture / video / document name (photo.jpg.exe, reversed names), content that
//              doesn't match the name (a "PDF" that is really a program), risky things inside archives, programs that
//              nobody signed.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ddm {
struct Task;
namespace inspect {

enum class Level { Unknown = 0, Safe = 1, Caution = 2, Danger = 3 };

struct Finding { Level level = Level::Safe; std::string text; };

struct Report {
    Level level = Level::Unknown;   // the worst finding
    std::vector<Finding> findings;
    bool linkDone = false, fileDone = false;
    std::string signer;             // programs: who signed it ("" unknown / not signed)
    std::string summary() const;    // "Safe link" / "Be careful: …" / "Dangerous: …"
};

std::string levelName(Level l);

// The link alone (fast checks first; the domain's age needs the network and is cached a week).
Report checkLink(const std::string& url, const std::string& page, const std::string& fileName);
// Everything for a download: the link, then a look at the file on the server (first bytes, inside archives, signer).
// Runs in the background; get() returns what is known so far.
void start(const Task& t);
bool get(const std::string& taskId, Report& out);
// The finished file on disk: does its content match its name? "" = fine, else why it's dangerous.
std::string checkLocalFile(const std::string& path);
// The file's SHA-256, worked out while it downloads: DDM hashes the part that has arrived in one piece from the start
// (in the background, a little at a time). Call it again and again (the download window does, twice a second); it
// stops by itself when nobody asks for a while.
struct LiveHash { int64_t hashed = 0, total = -1; std::string sha256; bool done = false; std::string note; };
LiveHash liveHash(const Task& t);
// Background: the list of sites spreading malware (once a day, quietly).
void refreshLists();

} // namespace inspect
} // namespace ddm
