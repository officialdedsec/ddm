// DDM's tray icon, clicked: a small panel over the clock with every download live (like IDM's one icon for all):
// name, a progress bar, %, speed and time left. Click one to bring its window back.
#pragma once

namespace ddm {
namespace ui {

bool trayPanelWanted();   // anything to show (downloading, waiting, or a download window hidden in the tray)
void toggleTrayPanel();   // opens it at the tray icon (a click while it's open closes it)
void closeTrayPanel();

} // namespace ui
} // namespace ddm
