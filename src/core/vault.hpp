// Private vault: files encrypted with AES-256 (each with its own key), names and details encrypted too, stored in a
// hidden, unindexed folder that only the current Windows user can open. Two password slots always exist, so a decoy
// vault (a second password that opens a different, harmless vault) can't be told apart from the real one.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ddm {
namespace vault {

struct Item {
    std::string id;        // file name in the vault folder
    std::string name;      // original name
    int64_t size = 0;
    int64_t added = 0;     // unix ms
    std::string from;      // where it came from (a download's address), may be empty
};

bool isSetUp();
bool create(const std::string& password, std::string& error);
// Opens the vault the password belongs to (the real one or the decoy). false + error (wrong / locked for N s).
bool unlock(const std::string& password, std::string& error);
bool isUnlocked();
bool isDecoy();                        // the open vault is the decoy one
void lock();                           // forgets the keys and deletes every temporary copy
int64_t lockedForSeconds();            // after several wrong passwords
bool changePassword(const std::string& oldPw, const std::string& newPw, std::string& error);   // the open vault
bool setDecoy(const std::string& password, std::string& error);   // from the real vault only
bool removeDecoy(std::string& error);
bool hasDecoyPassword();               // the real vault knows whether a decoy password is set

std::vector<Item> list();              // the open vault
// Adds a file (works while locked: goes into the real vault). move = delete the original afterwards.
bool add(const std::string& path, bool move, std::string& error, const std::string& from = "", std::function<void(int64_t, int64_t)> progress = nullptr);
bool extract(const std::string& id, const std::string& destPath, std::string& error, std::function<void(int64_t, int64_t)> progress = nullptr);
bool readAll(const std::string& id, std::string& data, std::string& error, int64_t maxBytes = 256 * 1024 * 1024);   // small files, in memory
std::string openCopy(const std::string& id, std::string& error);   // a temporary copy to open in its program (deleted on lock)
bool remove(const std::string& id, std::string& error);
bool rename(const std::string& id, const std::string& newName, std::string& error);
void clearTemp();                      // at start-up and when locking

} // namespace vault
} // namespace ddm
