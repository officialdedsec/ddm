#include "i18n.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>

namespace ddm {
namespace i18n {
namespace {

struct Template {
    std::vector<std::string> lit;   // literal parts: lit[0] {slot[0]} lit[1] {slot[1]} … lit[n]
    std::vector<int> slot;          // which {k} each gap is (1-based)
    std::string out;                // the translation, with {k}
    size_t litLen = 0;              // letters that must match: the more, the better the match
    int letters = 0;                // real letters in the fixed parts
    std::string longest;            // its longest literal part: a quick test before matching
};

struct Table {
    Language lang;
    std::unordered_map<std::string, std::string> exact;
    std::vector<Template> templates;
};

std::mutex g_mu;
std::map<std::string, Table> g_tables;   // every language found, by code
bool g_loaded = false;
std::string g_code;                      // current ("" = English)
const Table* g_cur = nullptr;
std::unordered_map<std::string, std::string> g_cache;

std::string unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[++i];
            o += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else o += s[i];
    }
    return o;
}

int letters(const std::string& s) {
    int n = 0;
    for (unsigned char c : s) if (isalpha(c) || c >= 0x80) n++;
    return n;
}

// "English {1} text {2}" -> a template; false when it isn't one (or is too vague to be safe: needs real words)
bool makeTemplate(const std::string& en, const std::string& tr, Template& t) {
    t = Template();
    std::string cur;
    for (size_t i = 0; i < en.size(); i++) {
        if (en[i] == '{' && i + 2 < en.size() && en[i + 1] >= '1' && en[i + 1] <= '9' && en[i + 2] == '}') {
            if (!t.slot.empty() && cur.empty()) return false;   // {1}{2}: where would one end?
            t.lit.push_back(cur);
            cur.clear();
            t.slot.push_back(en[i + 1] - '0');
            i += 2;
        } else cur += en[i];
    }
    t.lit.push_back(cur);
    if (t.slot.empty()) return false;
    int words = 0;
    for (auto& l : t.lit) { t.litLen += l.size(); words += letters(l); if (l.size() > t.longest.size()) t.longest = l; }
    if (words < 3) return false;
    t.letters = words;
    t.out = tr;
    return true;
}

void parse(const std::string& text, Table& tb, bool headersOnly) {
    size_t i = 0;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(i, e - i);
        i = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '@') {
            size_t sp = line.find(' ');
            std::string k = line.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
            std::string v = sp == std::string::npos ? "" : trim(line.substr(sp + 1));
            if (k == "code") tb.lang.code = v;
            else if (k == "name") tb.lang.name = v;
            else if (k == "english") tb.lang.english = v;
            else if (k == "rtl") tb.lang.rtl = v == "1" || v == "yes" || v == "true";
            continue;
        }
        if (headersOnly) continue;
        size_t a = line.find(" => ");
        if (a == std::string::npos) continue;
        std::string en = unescape(line.substr(0, a)), to = unescape(line.substr(a + 4));
        if (en.empty() || to.empty()) continue;
        Template t;
        if (makeTemplate(en, to, t)) tb.templates.push_back(t);
        else tb.exact[en] = to;
    }
}

std::string resourceText(const wchar_t* name) {
    HRSRC r = FindResourceW(nullptr, name, RT_RCDATA);
    if (!r) return "";
    HGLOBAL g = LoadResource(nullptr, r);
    auto p = static_cast<const char*>(LockResource(g));
    return p ? std::string(p, SizeofResource(nullptr, r)) : "";
}

BOOL CALLBACK onResource(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR param) {
    if (IS_INTRESOURCE(name)) return TRUE;
    std::wstring n = name;
    if (n.compare(0, 5, L"LANG_") == 0) reinterpret_cast<std::vector<std::wstring>*>(param)->push_back(n);
    return TRUE;
}

void merge(const std::string& text) {
    Table t;
    parse(text, t, false);
    if (t.lang.code.empty() || t.lang.code == "en") return;
    Table& into = g_tables[t.lang.code];
    if (into.lang.code.empty()) into.lang = t.lang;
    if (!t.lang.name.empty()) into.lang.name = t.lang.name;
    if (!t.lang.english.empty()) into.lang.english = t.lang.english;
    into.lang.rtl = t.lang.rtl || into.lang.rtl;
    for (auto& kv : t.exact) into.exact[kv.first] = kv.second;   // the user's own file wins
    for (auto& tp : t.templates) {
        auto same = std::find_if(into.templates.begin(), into.templates.end(), [&](const Template& x) { return x.lit == tp.lit && x.slot == tp.slot; });
        if (same != into.templates.end()) *same = tp;
        else into.templates.push_back(tp);
    }
}

void loadAll() {   // (g_mu held)
    if (g_loaded) return;
    g_loaded = true;
    std::vector<std::wstring> names;
    EnumResourceNamesW(nullptr, RT_RCDATA, onResource, reinterpret_cast<LONG_PTR>(&names));
    for (auto& n : names) merge(resourceText(n.c_str()));
    std::string dir = joinPath(dataDir(), "lang");   // your own or corrected translations
    for (auto& f : listDir(dir)) {
        if (lower(extOf(f)) != "txt") continue;
        std::string text;
        if (readFile(joinPath(dir, f), text) && text.size() < 8 * 1024 * 1024) merge(text);
    }
    for (auto& kv : g_tables)   // longest first: the most specific sentence wins when two fit
        std::stable_sort(kv.second.templates.begin(), kv.second.templates.end(), [](const Template& a, const Template& b) { return a.litLen > b.litLen; });
}

// does `s` fit the template? caps = the changing parts
bool fit(const Template& t, const std::string& s, size_t li, size_t pos, std::vector<std::string>& caps, int& budget) {
    if (--budget < 0) return false;
    if (li == t.lit.size() - 1) {   // the last literal ends the text
        const std::string& last = t.lit.back();
        if (s.size() < pos + last.size() || s.compare(s.size() - last.size(), last.size(), last) != 0) return false;
        size_t end = s.size() - last.size();
        if (end <= pos) return false;
        caps[li - 1] = s.substr(pos, end - pos);
        return true;
    }
    const std::string& next = t.lit[li];
    for (size_t at = s.find(next, pos + 1); at != std::string::npos; at = s.find(next, at + 1)) {
        caps[li - 1] = s.substr(pos, at - pos);
        if (fit(t, s, li + 1, at + next.size(), caps, budget)) return true;
    }
    return false;
}

enum Mode { Full = 0, NameOnly = 1, InSentence = 2 };   // InSentence: the changing part of a template
std::string translate(const Table& tb, const std::string& s, int depth, int mode = Full);

bool byTemplate(const Table& tb, const std::string& s, int depth, int mode, std::string& out) {
    for (auto& t : tb.templates) {
        if (t.litLen >= s.size()) continue;
        if (mode == InSentence && t.letters < 10) continue;   // a sentence's changing part is usually a name: only clear sentences there
        if (!t.longest.empty() && s.find(t.longest) == std::string::npos) continue;
        if (s.compare(0, t.lit[0].size(), t.lit[0]) != 0) continue;
        std::vector<std::string> caps(t.slot.size());
        int budget = 200;
        if (!fit(t, s, 1, t.lit[0].size(), caps, budget)) continue;
        // a short template that starts with its changing part ("{1} downloads", "{1} video{2}") is a count: that part
        // must hold a number, or any text with the word in it would be changed ("Night downloads", "The video …")
        if (t.lit[0].empty() && t.letters < 10 && caps[0].find_first_of("0123456789") == std::string::npos) continue;
        out.clear();
        for (size_t i = 0; i < t.out.size(); i++) {
            if (t.out[i] == '{' && i + 2 < t.out.size() && t.out[i + 1] >= '1' && t.out[i + 1] <= '9' && t.out[i + 2] == '}') {
                int k = t.out[i + 1] - '0';
                for (size_t g = 0; g < t.slot.size(); g++)
                    if (t.slot[g] == k) { out += depth < 3 ? translate(tb, caps[g], depth + 1, InSentence) : caps[g]; break; }
                i += 2;
            } else out += t.out[i];
        }
        return true;
    }
    return false;
}

// pieces joined by a separator, each translated on its own ("MP4 · video + sound", paragraphs)
bool byPieces(const Table& tb, const std::string& s, const std::string& sep, int depth, int mode, std::string& out) {
    if (s.find(sep) == std::string::npos) return false;
    bool changed = false;
    out.clear();
    size_t i = 0;
    while (true) {
        size_t e = s.find(sep, i);
        std::string piece = s.substr(i, e == std::string::npos ? std::string::npos : e - i);
        std::string t = piece.empty() ? piece : translate(tb, piece, depth + 1, mode);
        if (t != piece) changed = true;
        out += t;
        if (e == std::string::npos) break;
        out += sep;
        i = e + sep.size();
    }
    return changed;
}

std::string translate(const Table& tb, const std::string& s, int depth, int mode) {
    bool exactOnly = mode == NameOnly;
    if (s.empty() || depth > 4) return s;
    auto it = tb.exact.find(s);
    if (it != tb.exact.end()) return it->second;
    {   // the same words with spaces, a colon or "…" around them
        size_t a = s.find_first_not_of(" "), b = s.find_last_not_of(" ");
        if (a != std::string::npos && (a > 0 || b + 1 < s.size())) {
            std::string core = s.substr(a, b - a + 1), t = translate(tb, core, depth + 1, mode);
            if (t != core) return s.substr(0, a) + t + s.substr(b + 1);
        }
        for (const char* tail : {":", "…", ".", "!", "?"}) {
            size_t n = strlen(tail);
            if (s.size() > n && s.compare(s.size() - n, n, tail) == 0) {
                auto e2 = tb.exact.find(s.substr(0, s.size() - n));
                if (e2 != tb.exact.end()) return e2->second + tail;
            }
        }
    }
    if (exactOnly) return s;   // a name: only when it is one of DDM's own words as a whole
    std::string out;
    if (byTemplate(tb, s, depth, mode, out)) return out;
    for (const char* sep : {"\n", " · ", " — ", " | ", ": "})
        if (byPieces(tb, s, sep, depth, mode, out)) return out;
    return s;
}

} // namespace

std::vector<Language> languages() {
    std::lock_guard<std::mutex> l(g_mu);
    loadAll();
    std::vector<Language> v;
    Language en;
    en.code = "en"; en.name = "English"; en.english = "English";
    v.push_back(en);
    for (auto& kv : g_tables) v.push_back(kv.second.lang);
    std::sort(v.begin() + 1, v.end(), [](const Language& a, const Language& b) { return a.english < b.english; });
    return v;
}

std::string systemLanguage() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = L"";
    typedef int(WINAPI * L2N)(LCID, LPWSTR, int, DWORD);
    auto f = reinterpret_cast<L2N>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LCIDToLocaleName"));
    if (!f || !f(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0)) return "";
    std::string full = U(name);                 // "pt-BR", "bn-BD"
    std::string base = full.substr(0, full.find('-'));
    std::lock_guard<std::mutex> l(g_mu);
    loadAll();
    if (g_tables.count(full)) return full;
    if (g_tables.count(base)) return base;
    for (auto& kv : g_tables) if (kv.first.substr(0, kv.first.find('-')) == base) return kv.first;   // pt -> pt-BR
    return "";
}

void setLanguage(const std::string& code) {
    std::lock_guard<std::mutex> l(g_mu);
    loadAll();
    auto it = g_tables.find(code);
    g_cur = it == g_tables.end() ? nullptr : &it->second;
    g_code = g_cur ? code : "";
    g_cache.clear();
}

const std::string& current() { static std::string en = "en"; return g_code.empty() ? en : g_code; }
bool rtl() { return g_cur && g_cur->lang.rtl; }

std::string tr(const std::string& s) {
    if (!g_cur || s.empty()) return s;
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_cur) return s;
    auto it = g_cache.find(s);
    if (it != g_cache.end()) return it->second;
    std::string t = translate(*g_cur, s, 0);
    if (g_cache.size() > 20000) g_cache.clear();
    g_cache.emplace(s, t);
    return t;
}

std::string trName(const std::string& s) {
    if (!g_cur || s.empty()) return s;
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_cur) return s;
    return translate(*g_cur, s, 0, NameOnly);
}

Coverage coverage() {
    std::lock_guard<std::mutex> l(g_mu);
    Coverage c;
    if (g_cur) { c.entries = static_cast<int>(g_cur->exact.size()); c.templates = static_cast<int>(g_cur->templates.size()); }
    return c;
}

} // namespace i18n
} // namespace ddm
