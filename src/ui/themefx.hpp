// Switching between light and dark: the new look spreads out in a soft circle from where you clicked,
// over every DDM window at once (Windows' "Show animations" off: it simply changes).
#pragma once
#include <functional>

namespace ddm {
namespace ui {

void themeTransition(const std::function<void()>& change);

} // namespace ui
} // namespace ddm
