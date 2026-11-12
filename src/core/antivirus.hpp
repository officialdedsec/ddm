// Scanning finished downloads with the antivirus: Windows' own attachment scanning (every registered antivirus, on
// Windows 7 to 11), Microsoft Defender's command-line scanner when it is the active antivirus, or a program the user picks.
#pragma once
#include <functional>
#include <string>

namespace ddm {
namespace antivirus {

struct Result {
    enum Kind { Clean, Threat, Removed, Error, Skipped } kind = Skipped;
    std::string scanner;   // "Microsoft Defender", "Avast Antivirus", …
    std::string message;   // friendly line for the user
};

std::string activeName();          // the antivirus Windows reports as active ("" when unknown)
std::string defenderPath();        // MpCmdRun.exe, or ""
std::string scannerName();         // what DDM will use with the current options
// Scans a finished file or folder (blocking; call from a worker thread). url / referrer are given to Windows'
// attachment check so it can apply its zone rules.
Result scan(const std::string& path, const std::string& url = "", const std::string& referrer = "");
bool wantsScan(const std::string& fileName);   // Options: every file, or programs and archives only

} // namespace antivirus
} // namespace ddm
