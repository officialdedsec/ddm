// The speed to draw right now.
//
// The engine reports a new figure a few times a second. Drawn exactly as it comes, the number steps, and
// because the window repaints on a clock of its own the two drift apart: sometimes the same figure is drawn
// twice (the number looks stuck) and sometimes one is missed (it looks like a jump). So the drawn figure eases
// towards the reported one instead, and the window repaints often enough to show that movement.
//
// Call it once wherever the speed is drawn, with the download's id as the key (or a name of your own for a
// total). Calling it twice in the same repaint is harmless: the easing goes by the clock, not by the call.
#pragma once
#include <string>

namespace ddm {
namespace ui {

double glidedSpeed(const std::string& key, double target);   // bytes per second, eased

} // namespace ui
} // namespace ddm
