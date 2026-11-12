// The encrypted .ddm download-list format (same as DDM 3.x): "DDMLIST1" | IV | AES-256-CBC | HMAC-SHA256.
#pragma once
#include <string>
#include <vector>

namespace ddm {
namespace listvault {
std::vector<unsigned char> encrypt(const std::string& json);
bool decrypt(const std::vector<unsigned char>& blob, std::string& json, std::string& error);
bool looksLikeDdm(const std::string& data);
} // namespace listvault
} // namespace ddm
