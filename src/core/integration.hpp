// Browser integration: registers DDM's browser helper for Chrome, Edge, Brave, Vivaldi, Opera, Yandex, Chromium and
// Firefox (current user, no admin), offers the store extension, and knows which extension IDs may talk to DDM.
#pragma once
#include <string>
#include <vector>

namespace ddm {
namespace integration {

const char* const kHostName = "com.dedseec.ddm";
const char* const kFirefoxId = "ddm@dedseec.com";
const char* const kChromiumId = "kjfgjaeahlebagpibnmcppihinbbgfec";   // unpacked extension (fixed by its manifest key)

void registerHost();          // every start: writes the helper manifests + registry entries
void unregisterAll();         // uninstall
void setStartWithWindows(bool on);
void refreshStoreConfig();    // signed stores.json from dedseec.com (worker thread)
std::vector<std::string> allowedExtensionIds();
std::string chromeStoreId();
std::string edgeStoreId();
std::string chromeListing();
std::string edgeListing();
std::string firefoxListing();

// Other download managers on this PC ("Internet Download Manager", "Free Download Manager", …).
std::vector<std::string> otherDownloaders();
// Makes DDM the program that opens magnet links (current user). Windows 10/11 may still ask once which app to use.
void claimMagnetLinks();
// Opens the browser's own extensions page (to turn another downloader's extension off). false = couldn't.
bool openExtensionsPage(const std::string& browserKey);

struct BrowserInfo { std::string name, key; bool installed = false; std::string exe; };
std::vector<BrowserInfo> browsers();   // which browsers are on this PC (Windows' list of browsers, App Paths, install folders)
std::string browserExe(const std::string& browserKey);    // its program ("" = not found)
bool isFirefoxFamily(const std::string& browserKey);      // Firefox, Waterfox, LibreWolf, Floorp (Firefox add-ons)
bool openInBrowser(const std::string& browserKey, const std::string& url);   // in that browser, not the default one; false = couldn't
// DDM's own add-on loaded by hand ("Load unpacked" / developer mode) in Chrome, Edge, Brave, Vivaldi or Opera: its ID
// is random, so DDM looks for it in the browsers' profiles and allows it — only when every file in its folder is
// exactly DDM's (checked against fingerprints made when DDM was built). Runs quietly in the background and updates the
// browser helper's list by itself.
std::vector<std::string> handLoadedAddonIds();
void watchHandLoadedAddons();
// Is DDM's add-on in this browser (any of its profiles)? Looks at the browser's own folders, nothing is sent.
bool addonInstalled(const std::string& browserKey);
std::string listingFor(const std::string& browserKey);   // the store page to get the add-on for that browser

} // namespace integration
} // namespace ddm
