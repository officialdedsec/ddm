// The dust effect (like Telegram's delete): a window or some list rows turn into fine dust that blows up and to the
// right and fades, sweeping from left to right. It plays on its own click-through, always-on-top layered window and
// thread, so the window itself can close, hide or minimize at once and nothing waits for the animation.
// Off when Options → General says so, when Windows' "Show animations in Windows" is off, and in Remote Desktop.
#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>

namespace ddm {
namespace ui {
namespace dust {

bool enabled();

// A window that is about to close, hide or minimize: its picture as it is now turns to dust where it stands.
// Returns once the dust has taken the window's place on screen (the caller then hides / destroys / minimizes it, and
// Windows' own fade or minimize animation is skipped that one time). Returns false (and does nothing) when off.
bool window(HWND h);

// A picture (32-bit BGRA, straight alpha; transparent pixels are not dust) shown at a screen position, then dusted.
bool image(std::vector<uint32_t> bgra, int w, int h, POINT screen, int durationMs);

int durationMs();          // how long the dust flies (the list waits about half of it before closing the gap)

} // namespace dust
} // namespace ui
} // namespace ddm
