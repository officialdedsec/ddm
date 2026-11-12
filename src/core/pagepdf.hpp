// Right-click → "Save this page as PDF with DDM": the page is printed to a PDF by the Edge or Chrome already on this
// PC, run hidden with a fresh profile of DDM's own for each job. When the add-on sends a snapshot of the page as you
// see it (signed in, expanded, scrolled — its scripts removed), that snapshot is printed; otherwise the address is.
#pragma once
#include <string>

namespace ddm {
namespace pagepdf {
std::string browserExe();   // Edge or Chrome on this PC ("" = none)
// Saves the page as a PDF at outPath. html = the add-on's snapshot ("" = print the address). false + error when it can't.
bool save(const std::string& url, const std::string& html, const std::string& outPath, std::string& error);
} // namespace pagepdf
} // namespace ddm
