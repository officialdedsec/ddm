// Site rules that update by themselves: when a site changes how it shows its videos, DedSec publishes new rules
// (sites.json, signed with DedSec's key like update.json) and every DDM picks them up within hours — no new DDM
// version needed. Rules are only data (where in a site's page or API the video addresses are); a rule that isn't
// signed by DedSec is never used.
#pragma once
#include "../common/json.hpp"
#include <string>

namespace ddm {
namespace siterules {

// The current rules (the newest signed copy: downloaded, or kept from before). Thread-safe copy.
Json rules();
int version();
// Fetches newer rules when the last check is older than maxAgeHours (network; any thread). true = changed.
bool refresh(double maxAgeHours = 6);
// For tests: rules from a file, checked the same way (its .sig next to it).
bool loadFile(const std::string& path);

} // namespace siterules
} // namespace ddm
