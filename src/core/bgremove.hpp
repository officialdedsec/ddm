// Tools → Remove image background: DDM's own small neural network (U²-Net, the compact "u2netp" weights, Apache-2.0)
// runs on the PC's processor — no upload, no outside program. It finds the subject, its edges are refined against
// the picture itself, and a PNG with a transparent background is saved.
#pragma once
#include <atomic>
#include <functional>
#include <string>

namespace ddm {
namespace bgremove {

// progress(0…1, text). Returns the saved PNG ("" = failed, error says why).
std::string removeBackground(const std::string& picture, std::function<void(double, const std::string&)> progress, std::atomic<bool>* stop, std::string& error);

// The subject's mask only (0…255 per pixel, the picture's size): for tests.
bool subjectMask(const std::string& picture, std::string& mask, int& width, int& height, std::string& error);

} // namespace bgremove
} // namespace ddm
