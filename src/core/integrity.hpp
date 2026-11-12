// The official DDM.exe is signed by DedSec: a DDM.exe.sig is shipped next to it, and a DDM.exe that was changed
// afterwards (by malware, say) refuses to start. A build of your own has no DDM.exe.sig and simply runs.
#pragma once

namespace ddm {
namespace integrity {

bool hostOk();   // DDM.exe unchanged (when a DDM.exe.sig is shipped next to it)

} // namespace integrity
} // namespace ddm
