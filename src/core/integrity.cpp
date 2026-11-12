#include "integrity.hpp"
#include "crypto.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <string>

namespace ddm {
namespace integrity {

bool hostOk() {
    std::string exe = joinPath(exeDir(), "DDM.exe");
    wchar_t self[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    if (*self) exe = U(self);
    std::string sigText;
    if (!readFile(exe + ".sig", sigText)) return true;   // a build of your own: no signature shipped
    crypto::Bytes h = crypto::hashFile(exe, BCRYPT_SHA256_ALGORITHM);
    if (h.size() != 32) return false;
    // what was signed: "ddm-host-v1|<lowercase sha256 hex of DDM.exe>"
    static const char* d = "0123456789abcdef";
    std::string msg = "ddm-host-v1|";
    for (unsigned char c : h) { msg += d[(c >> 4) & 15]; msg += d[c & 15]; }
    crypto::Bytes sig(sigText.begin(), sigText.end());
    return crypto::verifyDdm(msg.data(), msg.size(), sig);
}

} // namespace integrity
} // namespace ddm
