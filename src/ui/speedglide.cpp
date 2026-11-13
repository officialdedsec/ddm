#include "speedglide.hpp"
#include "../common/util.hpp"
#include <cmath>
#include <map>

namespace ddm {
namespace ui {
namespace {

struct Shown { double value = 0; int64_t at = 0; };
std::map<std::string, Shown> g_shown;

} // namespace

double glidedSpeed(const std::string& key, double target) {
    if (target <= 0) { g_shown.erase(key); return 0; }   // paused, finished or not started: the number goes at once
    int64_t now = nowMs();
    if (g_shown.size() > 400) g_shown.clear();           // (a very long list: start them all afresh rather than grow)
    auto it = g_shown.find(key);
    if (it == g_shown.end()) { Shown s; s.value = target; s.at = now; g_shown[key] = s; return target; }
    Shown& s = it->second;
    int64_t dt = now - s.at;
    if (dt <= 0) return s.value;                         // twice in the same repaint
    s.at = now;
    if (dt > 4000) s.value = target;                     // out of sight for a while: begin again where it is now
    else s.value += (target - s.value) * (1.0 - std::exp(-static_cast<double>(dt) / 220.0));
    return s.value;
}

} // namespace ui
} // namespace ddm
