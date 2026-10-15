// Minimal JSON DOM: ordered objects, int-preserving numbers, pretty printer.
#pragma once
#include "core/base.h"

namespace fo {

class Json {
public:
    enum class Type : u8 { Null, Bool, Int, Double, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : m_type(Type::Bool), m_int(b ? 1 : 0) {}
    Json(int v) : m_type(Type::Int), m_int(v) {}
    Json(unsigned v) : m_type(Type::Int), m_int(v) {}
    Json(i64 v) : m_type(Type::Int), m_int(v) {}
    Json(u64 v) : m_type(Type::Int), m_int((i64)v) {}
    Json(double v) : m_type(Type::Double), m_double(v) {}
    Json(const char* s) : m_type(Type::String), m_str(s) {}
    Json(const std::string& s) : m_type(Type::String), m_str(s) {}
    Json(std::string&& s) : m_type(Type::String), m_str(std::move(s)) {}

    static Json array() { Json j; j.m_type = Type::Array; return j; }
    static Json object() { Json j; j.m_type = Type::Object; return j; }

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Int || m_type == Type::Double; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    // Lenient accessors with defaults. Strings like "0x1F" are accepted as integers.
    i64 i(i64 def = 0) const;
    int toInt(int def = 0) const { return (int)i(def); }
    double d(double def = 0.0) const;
    bool b(bool def = false) const;
    std::string str(const std::string& def = "") const;

    // Array access
    size_t size() const;
    const Json& operator[](size_t idx) const;
    Json& at(size_t idx);
    void push(Json v);
    const std::vector<Json>& items() const { return m_arr; }
    std::vector<Json>& items() { return m_arr; }

    // Object access (missing keys yield a shared null value)
    const Json& operator[](const std::string& key) const;
    const Json& operator[](const char* key) const { return (*this)[std::string(key)]; }
    Json& set(const std::string& key, Json v);
    Json* find(const std::string& key);
    const Json* find(const std::string& key) const;
    bool has(const std::string& key) const { return find(key) != nullptr; }
    const std::vector<std::pair<std::string, Json>>& members() const { return m_obj; }

    static Json parse(const std::string& text, std::string* error = nullptr);
    // indent < 0 -> compact. Arrays containing only scalars are written on one line (wrapped).
    std::string dump(int indent = 2) const;

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type m_type = Type::Null;
    i64 m_int = 0;
    double m_double = 0.0;
    std::string m_str;
    std::vector<Json> m_arr;
    std::vector<std::pair<std::string, Json>> m_obj;
};

std::string jsonHex(u32 v); // "0x01000016" formatting helper

} // namespace fo
