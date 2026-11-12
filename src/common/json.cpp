#include "json.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ddm {

static const Json kNull;

const Json& Json::operator[](const std::string& key) const {
    if (type_ != Type::Object) return kNull;
    auto it = obj_.find(key);
    return it == obj_.end() ? kNull : it->second;
}

Json& Json::operator[](const std::string& key) {
    if (type_ != Type::Object) { *this = object(); }
    return obj_[key];
}

bool Json::has(const std::string& key) const { return type_ == Type::Object && obj_.count(key) > 0; }

const Json& Json::at(size_t i) const { return (type_ == Type::Array && i < arr_.size()) ? arr_[i] : kNull; }

void Json::push(Json v) {
    if (type_ != Type::Array) { *this = array(); }
    arr_.push_back(std::move(v));
}

std::string jsonQuote(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
        }
    }
    o += '"';
    return o;
}

void Json::write(std::string& out, bool pretty, int indent) const {
    auto nl = [&](int ind) { if (pretty) { out += '\n'; out.append(static_cast<size_t>(ind) * 2, ' '); } };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += b_ ? "true" : "false"; break;
        case Type::Number: {
            if (std::isfinite(n_) && n_ == std::floor(n_) && std::fabs(n_) < 9.007199254740992e15) {
                char b[32]; snprintf(b, sizeof b, "%lld", static_cast<long long>(n_)); out += b;
            } else if (std::isfinite(n_)) {
                char b[40]; snprintf(b, sizeof b, "%.10g", n_); out += b;
            } else out += "0";
            break;
        }
        case Type::String: out += jsonQuote(s_); break;
        case Type::Array: {
            out += '[';
            bool first = true;
            for (auto& v : arr_) { if (!first) out += ','; first = false; nl(indent + 1); v.write(out, pretty, indent + 1); }
            if (!arr_.empty()) nl(indent);
            out += ']';
            break;
        }
        case Type::Object: {
            out += '{';
            bool first = true;
            for (auto& kv : obj_) {
                if (!first) out += ',';
                first = false;
                nl(indent + 1);
                out += jsonQuote(kv.first);
                out += pretty ? ": " : ":";
                kv.second.write(out, pretty, indent + 1);
            }
            if (!obj_.empty()) nl(indent);
            out += '}';
            break;
        }
    }
}

std::string Json::dump(bool pretty) const { std::string o; write(o, pretty, 0); return o; }

namespace {
struct Parser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;
    int depth = 0;

    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++; }
    bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { i++; return true; } return false; }

    static void utf8(std::string& o, uint32_t cp) {
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    }

    uint32_t hex4() {
        if (i + 4 > s.size()) { ok = false; return 0; }
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else { ok = false; return 0; }
        }
        return v;
    }

    std::string string() {
        std::string o;
        i++; // opening quote
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return o;
            if (c != '\\') { o += c; continue; }
            if (i >= s.size()) break;
            char e = s[i++];
            switch (e) {
                case '"': o += '"'; break;
                case '\\': o += '\\'; break;
                case '/': o += '/'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'u': {
                    uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        uint32_t lo = hex4();
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(o, cp);
                    break;
                }
                default: o += e;
            }
        }
        ok = false;
        return o;
    }

    Json value() {
        if (++depth > 200) { ok = false; return Json(); }
        ws();
        Json r;
        if (i >= s.size()) { ok = false; }
        else if (s[i] == '{') {
            i++;
            r = Json::object();
            if (!eat('}')) {
                do {
                    ws();
                    if (i >= s.size() || s[i] != '"') { ok = false; break; }
                    std::string k = string();
                    if (!eat(':')) { ok = false; break; }
                    r[k] = value();
                } while (ok && eat(','));
                if (ok && !eat('}')) ok = false;
            }
        } else if (s[i] == '[') {
            i++;
            r = Json::array();
            if (!eat(']')) {
                do { r.push(value()); } while (ok && eat(','));
                if (ok && !eat(']')) ok = false;
            }
        } else if (s[i] == '"') {
            r = Json(string());
        } else if (s.compare(i, 4, "true") == 0) { i += 4; r = Json(true); }
        else if (s.compare(i, 5, "false") == 0) { i += 5; r = Json(false); }
        else if (s.compare(i, 4, "null") == 0) { i += 4; }
        else {
            const char* b = s.c_str() + i;
            char* e = nullptr;
            double d = strtod(b, &e);
            if (e == b) ok = false; else { i += static_cast<size_t>(e - b); r = Json(d); }
        }
        depth--;
        return r;
    }
};
} // namespace

Json Json::parse(const std::string& text, bool* ok) {
    Parser p{text};
    Json r = p.value();
    p.ws();
    if (p.i != text.size()) p.ok = false;
    if (ok) *ok = p.ok;
    return p.ok ? r : Json();
}

} // namespace ddm
