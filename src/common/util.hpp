// Strings, paths, files, formatting and URL helpers shared by the app and the browser helper.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct HINSTANCE__;   // (a Windows module handle, without <windows.h> here)

namespace ddm {

// UTF-8 <-> UTF-16 (Win32 boundary).
std::wstring W(const std::string& s);
std::string U(const std::wstring& s);

std::string lower(std::string s);
std::string trim(const std::string& s);
bool startsWith(const std::string& s, const std::string& p);
bool endsWith(const std::string& s, const std::string& p);
bool iequals(const std::string& a, const std::string& b);
std::vector<std::string> split(const std::string& s, char sep);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);

// Files (UTF-8 paths).
bool readFile(const std::string& path, std::string& out);
bool writeFileAtomic(const std::string& path, const std::string& data);   // write temp, then replace
bool fileExists(const std::string& path);
bool dirExists(const std::string& path);
bool makeDirs(const std::string& path);
std::vector<std::string> listDir(const std::string& dir);   // file and folder names (no . and ..)
void removeTree(const std::string& dir);                  // a folder with everything in it (permanently)
int64_t fileSize(const std::string& path);
std::string joinPath(const std::string& a, const std::string& b);
std::string dirName(const std::string& path);
std::string baseName(const std::string& path);
std::string extOf(const std::string& name);          // lower case, no dot: "zip"
std::string stem(const std::string& name);

// Well-known folders.
std::string exeDir();
::HINSTANCE__* systemDll(const wchar_t* name);   // a Windows DLL, from System32 only (never one planted in another folder)
std::string dataDir();       // %LOCALAPPDATA%\DDM\native (or <exe>\data when portable.txt exists)
std::string rootDataDir();   // %LOCALAPPDATA%\DDM (shared with earlier DDM versions)
std::string downloadsDir();     // the user's Downloads folder

// Names.
std::string sanitizeFileName(const std::string& name);
std::string uniquePath(const std::string& path);    // "a.zip" -> "a (2).zip" when taken

// URLs.
std::string urlHost(const std::string& url);       // "www.example.com" (lower case, no port)
std::string urlSite(const std::string& url);       // "example.com" (no www.)
std::string urlScheme(const std::string& url);
std::string urlDecode(const std::string& s);
std::string fileNameFromUrl(const std::string& url);
bool looksLikeUrl(const std::string& s);
std::vector<std::string> extractUrls(const std::string& text);   // http(s)/ftp/magnet/mega links in any text

// Formatting.
std::string formatBytes(int64_t n);
std::string formatSpeed(double bytesPerSec);
std::string formatKBps(double bytesPerSec);       // "1,536.4 KB/s" (as IDM shows it)
std::string groupThousands(int64_t n);            // 1536 -> "1,536"
std::string formatDuration(int64_t seconds);
std::string formatDate(int64_t unixMs);
std::string formatDay(int64_t unixMs);    // "Oct 30, 2027"
int64_t nowMs();
std::string newId();

// Diagnostics (data dir\ddm.log, small and rotating).
void logLine(const std::string& line);

} // namespace ddm
