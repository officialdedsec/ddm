#include "links.hpp"
#include "http.hpp"
#include "media.hpp"
#include "mega.hpp"
#include "torrent.hpp"
#include "../common/util.hpp"
#include <regex>
#include <set>

namespace ddm {
namespace links {

namespace {
const std::set<std::string> kFileExt = {"zip", "rar", "7z", "tar", "gz", "bz2", "xz", "zst", "apk", "xapk", "apks", "pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "txt", "csv",
    "epub", "mobi", "mp4", "mkv", "webm", "avi", "mov", "m4v", "3gp", "mp3", "m4a", "aac", "flac", "wav", "ogg", "opus", "jpg", "jpeg", "png", "gif", "webp", "iso", "img",
    "exe", "msi", "msix", "appx", "dmg", "bin", "deb", "rpm", "msu", "cab", "wmv", "flv", "ts", "torrent"};

std::string htmlDecode(std::string s) {
    s = replaceAll(s, "&amp;", "&"); s = replaceAll(s, "&quot;", "\""); s = replaceAll(s, "&#39;", "'"); s = replaceAll(s, "&lt;", "<"); s = replaceAll(s, "&gt;", ">");
    return s;
}
std::string esc(const std::string& v) {
    std::string o;
    for (unsigned char c : v) { if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') o += static_cast<char>(c); else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; } }
    return o;
}
std::string driveId(const std::string& url) {
    static const std::regex re("(?:/file/d/|/d/|[?&](?:id|ids)=|/uc\\?[^#]*\\bid=)([A-Za-z0-9_\\-]{10,})", std::regex::icase);
    std::smatch m;
    return std::regex_search(url, m, re) ? std::string(m[1]) : "";
}
} // namespace

Type classify(const std::string& url) {
    if (isMagnet(url)) return Type::Magnet;
    if (isMegaLink(url)) return Type::Mega;
    if (isDrive(url)) return Type::DirectFile;
    std::string path = url;
    size_t p = path.find("://");
    if (p != std::string::npos) { size_t sl = path.find('/', p + 3); path = sl == std::string::npos ? "" : path.substr(sl); }
    size_t q = path.find_first_of("?#");
    if (q != std::string::npos) path = path.substr(0, q);
    path = lower(path);
    if (endsWith(path, ".torrent")) return Type::TorrentFile;
    if (isVideoSite(url)) return Type::Video;
    std::string last = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
    size_t dot = last.rfind('.');
    if (dot != std::string::npos && kFileExt.count(last.substr(dot + 1))) return Type::DirectFile;
    return Type::Web;
}

std::string magnetName(const std::string& uri) {
    static const std::regex re("[?&]dn=([^&]+)", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(uri, m, re)) return "";
    return urlDecode(replaceAll(m[1], "+", " "));
}

bool isDrive(const std::string& url) {
    std::string h = lower(urlHost(url));
    return (h == "drive.google.com" || h == "drive.usercontent.google.com" || h == "docs.google.com") && !driveId(url).empty();
}

std::string resolveDrive(const std::string& url, const std::string& cookies) {
    std::string id = driveId(url);
    if (id.empty()) return url;
    std::string direct = "https://drive.usercontent.google.com/download?id=" + id + "&export=download";
    RequestInfo r;
    r.url = direct + "&confirm=t";
    r.cookies = cookies;
    ResponseInfo ri;
    if (probe(r, ri) && ri.status < 400 && ri.mime.find("text/html") == std::string::npos) return direct + "&confirm=t";
    r.url = direct;
    std::string html;
    if (http::get(r, html, nullptr, 512 * 1024, 30) == 200) {
        // the form is cut out by hand: a regular expression run over a whole page goes very deep (a big page could crash it)
        static const std::regex form("^<form[^>]*\\baction=\"([^\"]+)\"[^>]*>$", std::regex::icase);
        static const std::regex input("<input\\b[^>]*?\\bname=\"([^\"]+)\"[^>]*?\\bvalue=\"([^\"]*)\"", std::regex::icase);
        std::smatch fm;
        std::string lowHtml = lower(html), formTag, inner;
        bool found = false;
        for (size_t fa = lowHtml.find("<form"), n = 0; fa != std::string::npos && n < 20 && !found; fa = lowHtml.find("<form", fa + 5), n++) {   // the first form with an action
            size_t te = lowHtml.find('>', fa), fb = lowHtml.find("</form>", fa);
            if (te == std::string::npos || fb == std::string::npos || te > fb) break;
            if (te - fa > 2048) continue;
            formTag = html.substr(fa, te + 1 - fa);
            found = std::regex_search(formTag, fm, form);
            if (found) inner = html.substr(te + 1, std::min<size_t>(fb - te - 1, 65536));
        }
        if (found) {
            std::string action = htmlDecode(fm[1]);
            if (startsWith(lower(action), "https://") && (lower(urlHost(action)) == "google.com" || endsWith(lower(urlHost(action)), ".google.com"))) {
                std::string out = action;
                bool first = action.find('?') == std::string::npos;
                for (auto it = std::sregex_iterator(inner.begin(), inner.end(), input); it != std::sregex_iterator(); ++it) {
                    out += (first ? "?" : "&");
                    first = false;
                    out += esc((*it)[1]) + "=" + esc(htmlDecode((*it)[2]));
                }
                return out;
            }
        }
    }
    return direct + "&confirm=t";
}

} // namespace links
} // namespace ddm
