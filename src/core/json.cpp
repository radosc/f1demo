#include "core/json.h"

namespace fo {

static const Json kNullJson;

std::string jsonHex(u32 v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

i64 Json::i(i64 def) const
{
    switch (m_type) {
    case Type::Int: return m_int;
    case Type::Bool: return m_int;
    case Type::Double: return (i64)m_double;
    case Type::String: {
        if (m_str.empty()) return def;
        char* end = nullptr;
        const char* s = m_str.c_str();
        i64 v;
        if (m_str.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) v = (i64)strtoull(s + 2, &end, 16);
        else if (m_str.size() > 3 && s[0] == '-' && s[1] == '0' && (s[2] == 'x' || s[2] == 'X')) v = -(i64)strtoull(s + 3, &end, 16);
        else v = strtoll(s, &end, 10);
        return (end && *end == '\0') ? v : def;
    }
    default: return def;
    }
}

double Json::d(double def) const
{
    switch (m_type) {
    case Type::Int: return (double)m_int;
    case Type::Double: return m_double;
    case Type::Bool: return (double)m_int;
    case Type::String: {
        char* end = nullptr;
        double v = strtod(m_str.c_str(), &end);
        return (end && *end == '\0' && !m_str.empty()) ? v : def;
    }
    default: return def;
    }
}

bool Json::b(bool def) const
{
    switch (m_type) {
    case Type::Bool: return m_int != 0;
    case Type::Int: return m_int != 0;
    case Type::Double: return m_double != 0.0;
    case Type::String: return m_str == "true" || m_str == "1" || m_str == "yes";
    default: return def;
    }
}

std::string Json::str(const std::string& def) const
{
    switch (m_type) {
    case Type::String: return m_str;
    case Type::Int: return std::to_string(m_int);
    case Type::Double: return strFormat("%g", m_double);
    case Type::Bool: return m_int ? "true" : "false";
    default: return def;
    }
}

size_t Json::size() const
{
    if (m_type == Type::Array) return m_arr.size();
    if (m_type == Type::Object) return m_obj.size();
    return 0;
}

const Json& Json::operator[](size_t idx) const
{
    if (m_type != Type::Array || idx >= m_arr.size()) return kNullJson;
    return m_arr[idx];
}

Json& Json::at(size_t idx) { return m_arr.at(idx); }

void Json::push(Json v)
{
    if (m_type != Type::Array) { *this = array(); }
    m_arr.push_back(std::move(v));
}

const Json& Json::operator[](const std::string& key) const
{
    const Json* f = find(key);
    return f ? *f : kNullJson;
}

Json& Json::set(const std::string& key, Json v)
{
    if (m_type != Type::Object) { *this = object(); }
    for (auto& kv : m_obj) {
        if (kv.first == key) { kv.second = std::move(v); return kv.second; }
    }
    m_obj.emplace_back(key, std::move(v));
    return m_obj.back().second;
}

Json* Json::find(const std::string& key)
{
    if (m_type != Type::Object) return nullptr;
    for (auto& kv : m_obj) if (kv.first == key) return &kv.second;
    return nullptr;
}

const Json* Json::find(const std::string& key) const
{
    if (m_type != Type::Object) return nullptr;
    for (auto& kv : m_obj) if (kv.first == key) return &kv.second;
    return nullptr;
}

// ------------------------------------------------------------------ parser
namespace {
struct Parser {
    const char* s;
    const char* e;
    const char* start;
    std::string err;

    void fail(const char* what)
    {
        if (!err.empty()) return;
        int line = 1, col = 1;
        for (const char* p = start; p < s; p++) {
            if (*p == '\n') { line++; col = 1; } else col++;
        }
        err = strFormat("JSON error at %d:%d: %s", line, col, what);
    }

    void ws()
    {
        while (s < e) {
            if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') { s++; continue; }
            // tolerate // and /* */ comments for hand-edited mod files
            if (*s == '/' && s + 1 < e && s[1] == '/') { while (s < e && *s != '\n') s++; continue; }
            if (*s == '/' && s + 1 < e && s[1] == '*') {
                s += 2;
                while (s + 1 < e && !(s[0] == '*' && s[1] == '/')) s++;
                s = std::min(e, s + 2);
                continue;
            }
            break;
        }
    }

    static void putUtf8(std::string& out, u32 cp)
    {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }

    bool parseString(std::string& out)
    {
        if (*s != '"') { fail("expected string"); return false; }
        s++;
        while (s < e && *s != '"') {
            char c = *s++;
            if (c == '\\') {
                if (s >= e) break;
                char x = *s++;
                switch (x) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (e - s < 4) { fail("bad \\u escape"); return false; }
                    u32 cp = (u32)strtoul(std::string(s, 4).c_str(), nullptr, 16);
                    s += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && e - s >= 6 && s[0] == '\\' && s[1] == 'u') {
                        u32 lo = (u32)strtoul(std::string(s + 2, 4).c_str(), nullptr, 16);
                        s += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    putUtf8(out, cp);
                    break;
                }
                default: out += x; break;
                }
            } else {
                out += c;
            }
        }
        if (s >= e) { fail("unterminated string"); return false; }
        s++;
        return true;
    }

    Json value(int depth)
    {
        if (depth > 256) { fail("nesting too deep"); return {}; }
        ws();
        if (s >= e) { fail("unexpected end"); return {}; }
        char c = *s;
        if (c == '{') {
            s++;
            Json obj = Json::object();
            ws();
            if (s < e && *s == '}') { s++; return obj; }
            while (s < e) {
                ws();
                std::string key;
                if (!parseString(key)) return {};
                ws();
                if (s >= e || *s != ':') { fail("expected ':'"); return {}; }
                s++;
                Json v = value(depth + 1);
                if (!err.empty()) return {};
                obj.set(key, std::move(v));
                ws();
                if (s < e && *s == ',') { s++; ws(); if (s < e && *s == '}') { s++; return obj; } continue; }
                if (s < e && *s == '}') { s++; return obj; }
                fail("expected ',' or '}'");
                return {};
            }
            fail("unterminated object");
            return {};
        }
        if (c == '[') {
            s++;
            Json arr = Json::array();
            ws();
            if (s < e && *s == ']') { s++; return arr; }
            while (s < e) {
                Json v = value(depth + 1);
                if (!err.empty()) return {};
                arr.push(std::move(v));
                ws();
                if (s < e && *s == ',') { s++; ws(); if (s < e && *s == ']') { s++; return arr; } continue; }
                if (s < e && *s == ']') { s++; return arr; }
                fail("expected ',' or ']'");
                return {};
            }
            fail("unterminated array");
            return {};
        }
        if (c == '"') {
            std::string str;
            if (!parseString(str)) return {};
            return Json(std::move(str));
        }
        if (c == 't' && e - s >= 4 && strncmp(s, "true", 4) == 0) { s += 4; return Json(true); }
        if (c == 'f' && e - s >= 5 && strncmp(s, "false", 5) == 0) { s += 5; return Json(false); }
        if (c == 'n' && e - s >= 4 && strncmp(s, "null", 4) == 0) { s += 4; return Json(); }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* b = s;
            bool isFloat = false;
            if (*s == '-') s++;
            if (s + 1 < e && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                s += 2;
                while (s < e && isxdigit((unsigned char)*s)) s++;
                std::string t(b, s);
                bool neg = t[0] == '-';
                i64 v = (i64)strtoull(t.c_str() + (neg ? 3 : 2), nullptr, 16);
                return Json(neg ? -v : v);
            }
            while (s < e && (isdigit((unsigned char)*s) || *s == '.' || *s == 'e' || *s == 'E' || *s == '+' || *s == '-')) {
                if (*s == '.' || *s == 'e' || *s == 'E') isFloat = true;
                s++;
            }
            std::string t(b, s);
            if (isFloat) return Json(strtod(t.c_str(), nullptr));
            return Json((i64)strtoll(t.c_str(), nullptr, 10));
        }
        fail("unexpected character");
        return {};
    }
};
} // namespace

Json Json::parse(const std::string& text, std::string* error)
{
    Parser p;
    p.start = p.s = text.data();
    p.e = text.data() + text.size();
    // skip UTF-8 BOM
    if (text.size() >= 3 && (u8)text[0] == 0xEF && (u8)text[1] == 0xBB && (u8)text[2] == 0xBF) p.s += 3;
    Json v = p.value(0);
    if (p.err.empty()) {
        p.ws();
        if (p.s != p.e) p.fail("trailing characters");
    }
    if (!p.err.empty()) {
        if (error) *error = p.err;
        return Json();
    }
    if (error) error->clear();
    return v;
}

// ------------------------------------------------------------------ writer
static void escapeTo(std::string& out, const std::string& s)
{
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) out += strFormat("\\u%04x", c);
            else out += (char)c;
        }
    }
    out += '"';
}

static bool isScalar(const Json& j) { return !j.isArray() && !j.isObject(); }

void Json::dumpTo(std::string& out, int indent, int depth) const
{
    switch (m_type) {
    case Type::Null: out += "null"; return;
    case Type::Bool: out += m_int ? "true" : "false"; return;
    case Type::Int: out += std::to_string(m_int); return;
    case Type::Double: {
        if (std::isfinite(m_double)) {
            std::string t = strFormat("%.9g", m_double);
            if (t.find_first_of(".eE") == std::string::npos) t += ".0";
            out += t;
        } else {
            out += "0";
        }
        return;
    }
    case Type::String: escapeTo(out, m_str); return;
    case Type::Array: {
        if (m_arr.empty()) { out += "[]"; return; }
        bool allScalar = std::all_of(m_arr.begin(), m_arr.end(), isScalar);
        if (indent < 0) {
            out += '[';
            for (size_t i = 0; i < m_arr.size(); i++) {
                if (i) out += ',';
                m_arr[i].dumpTo(out, indent, depth + 1);
            }
            out += ']';
            return;
        }
        if (allScalar) {
            // one line, wrapped every ~20 items to keep big tables readable
            out += '[';
            size_t lineStart = out.size();
            for (size_t i = 0; i < m_arr.size(); i++) {
                if (i) out += ',';
                if (out.size() - lineStart > 110) {
                    out += '\n';
                    out.append((size_t)(depth + 1) * indent, ' ');
                    lineStart = out.size();
                } else if (i) {
                    out += ' ';
                }
                m_arr[i].dumpTo(out, indent, depth + 1);
            }
            out += ']';
            return;
        }
        out += "[\n";
        for (size_t i = 0; i < m_arr.size(); i++) {
            out.append((size_t)(depth + 1) * indent, ' ');
            m_arr[i].dumpTo(out, indent, depth + 1);
            if (i + 1 < m_arr.size()) out += ',';
            out += '\n';
        }
        out.append((size_t)depth * indent, ' ');
        out += ']';
        return;
    }
    case Type::Object: {
        if (m_obj.empty()) { out += "{}"; return; }
        if (indent < 0) {
            out += '{';
            for (size_t i = 0; i < m_obj.size(); i++) {
                if (i) out += ',';
                escapeTo(out, m_obj[i].first);
                out += ':';
                m_obj[i].second.dumpTo(out, indent, depth + 1);
            }
            out += '}';
            return;
        }
        out += "{\n";
        for (size_t i = 0; i < m_obj.size(); i++) {
            out.append((size_t)(depth + 1) * indent, ' ');
            escapeTo(out, m_obj[i].first);
            out += ": ";
            m_obj[i].second.dumpTo(out, indent, depth + 1);
            if (i + 1 < m_obj.size()) out += ',';
            out += '\n';
        }
        out.append((size_t)depth * indent, ' ');
        out += '}';
        return;
    }
    }
}

std::string Json::dump(int indent) const
{
    std::string out;
    dumpTo(out, indent, 0);
    if (indent >= 0) out += '\n';
    return out;
}

} // namespace fo
