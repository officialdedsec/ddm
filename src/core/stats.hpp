// Statistics, counted on this PC only: data downloaded per day, per website, and finished downloads.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ddm {
namespace stats {

void addBytes(int64_t bytes);                                  // data that arrived (any download)
void addFinished(const std::string& site, int64_t size, int64_t millis);   // a download that finished
std::vector<std::pair<std::string, int64_t>> lastDays(int n);  // "2026-10-31" -> bytes, oldest first, every day present
std::vector<std::pair<std::string, int64_t>> topSites(int n);
int64_t total();
int64_t finishedCount();
int64_t fastest();                                             // best average speed of a finished download (bytes/s)
void clear();
void save();

} // namespace stats
} // namespace ddm
