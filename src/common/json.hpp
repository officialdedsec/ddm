// Small JSON value: parse and write (UTF-8). Enough for DDM's settings, download list and browser messages.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ddm {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), b_(b) {}
    Json(int v) : type_(Type::Number), n_(v) {}
    Json(int64_t v) : type_(Type::Number), n_(static_cast<double>(v)) {}
    Json(uint64_t v) : type_(Type::Number), n_(static_cast<double>(v)) {}
    Json(double v) : type_(Type::Number), n_(v) {}
    Json(const char* s) : type_(Type::String), s_(s ? s : "") {}
    Json(const std::string& s) : type_(Type::String), s_(s) {}
    Json(std::string&& s) : type_(Type::String), s_(std::move(s)) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }
    bool isString() const { return type_ == Type::String; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isBool() const { return type_ == Type::Bool; }

    // Typed reads with defaults (never throw).
    std::string str(const std::string& def = "") const { return type_ == Type::String ? s_ : def; }
    double num(double def = 0) const { return type_ == Type::Number ? n_ : (type_ == Type::Bool ? (b_ ? 1 : 0) : def); }
    int64_t i64(int64_t def = 0) const { return type_ == Type::Number ? static_cast<int64_t>(n_) : def; }
    int i32(int def = 0) const { return type_ == Type::Number ? static_cast<int>(n_) : def; }
    bool boolean(bool def = false) const { return type_ == Type::Bool ? b_ : (type_ == Type::Number ? n_ != 0 : def); }

    // Object access.
    const Json& operator[](const std::string& key) const;
    Json& operator[](const std::string& key);           // creates (turns null into object)
    bool has(const std::string& key) const;
    std::string str(const std::string& key, const std::string& def) const { return (*this)[key].str(def); }
    const std::map<std::string, Json>& items() const { return obj_; }
    void erase(const std::string& key) { obj_.erase(key); }

    // Array access.
    size_t size() const { return type_ == Type::Array ? arr_.size() : (type_ == Type::Object ? obj_.size() : 0); }
    const Json& at(size_t i) const;
    void push(Json v);
    const std::vector<Json>& list() const { return arr_; }

    std::string dump(bool pretty = false) const;
    static Json parse(const std::string& text, bool* ok = nullptr);

private:
    void write(std::string& out, bool pretty, int indent) const;
    Type type_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Json> arr_;
    std::map<std::string, Json> obj_;
};

std::string jsonQuote(const std::string& s);

} // namespace ddm
