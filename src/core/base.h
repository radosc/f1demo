// Common definitions shared by every module.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <map>
#include <set>
#include <functional>
#include <algorithm>
#include <utility>
#include <array>

namespace fo {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using Bytes = std::vector<u8>;

// ---------------------------------------------------------------- logging
void logInit(const std::string& path);
void logWrite(const char* level, const char* fmt, ...);
#define LOGI(...) ::fo::logWrite("INFO", __VA_ARGS__)
#define LOGW(...) ::fo::logWrite("WARN", __VA_ARGS__)
#define LOGE(...) ::fo::logWrite("ERROR", __VA_ARGS__)

// ---------------------------------------------------------------- strings
std::string strLower(std::string s);
std::string strUpper(std::string s);
std::string strTrim(const std::string& s);
std::vector<std::string> strSplit(const std::string& s, char sep);
bool strStartsWith(const std::string& s, const std::string& prefix);
bool strEndsWith(const std::string& s, const std::string& suffix);
bool strIEquals(const std::string& a, const std::string& b);
std::string strFormat(const char* fmt, ...);
std::string strReplaceAll(std::string s, const std::string& from, const std::string& to);
// Normalizes a virtual path: lower case, forward slashes, no leading slash, collapsed "./".
std::string pathNormalize(const std::string& p);
std::string pathDir(const std::string& p);   // "a/b/c.png" -> "a/b"
std::string pathFile(const std::string& p);  // "a/b/c.png" -> "c.png"
std::string pathStem(const std::string& p);  // "a/b/c.png" -> "c"
std::string pathExt(const std::string& p);   // "a/b/c.png" -> ".png"
std::string pathJoin(const std::string& a, const std::string& b);

std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& s);

// ---------------------------------------------------------------- binary
inline u16 rdBE16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }
inline u32 rdBE32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
inline i16 rdBE16s(const u8* p) { return (i16)rdBE16(p); }
inline i32 rdBE32s(const u8* p) { return (i32)rdBE32(p); }
inline u16 rdLE16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
inline u32 rdLE32(const u8* p) { return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
inline void wrBE32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
inline void wrLE16(u8* p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
inline void wrLE32(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

// Sequential big-endian reader with bounds checking; reading past the end yields zeros and sets `bad`.
struct BeReader {
    const u8* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    bool bad = false;
    BeReader() = default;
    BeReader(const u8* d, size_t n) : data(d), size(n) {}
    explicit BeReader(const Bytes& b) : data(b.data()), size(b.size()) {}
    bool can(size_t n) const { return pos + n <= size; }
    i32 i32v() { if (!can(4)) { bad = true; pos = size; return 0; } i32 v = rdBE32s(data + pos); pos += 4; return v; }
    u32 u32v() { return (u32)i32v(); }
    i16 i16v() { if (!can(2)) { bad = true; pos = size; return 0; } i16 v = rdBE16s(data + pos); pos += 2; return v; }
    u16 u16v() { return (u16)i16v(); }
    u8 u8v() { if (!can(1)) { bad = true; pos = size; return 0; } return data[pos++]; }
    void skip(size_t n) { if (!can(n)) { bad = true; pos = size; } else pos += n; }
    std::string str(size_t n) {
        if (!can(n)) { bad = true; pos = size; return {}; }
        std::string s((const char*)data + pos, n);
        pos += n;
        size_t z = s.find('\0');
        if (z != std::string::npos) s.resize(z);
        return s;
    }
};

// ---------------------------------------------------------------- misc
template <class T> inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

// Deterministic RNG (xorshift) used for game rolls; seeded from time.
struct Rng {
    u64 s = 0x9E3779B97F4A7C15ull;
    void seed(u64 v) { s = v ? v : 0x9E3779B97F4A7C15ull; }
    u32 next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (u32)(s >> 11); }
    // inclusive range
    int range(int lo, int hi) { if (hi <= lo) return lo; return lo + (int)(next() % (u32)(hi - lo + 1)); }
};
extern Rng g_rng;

double timeSeconds();     // monotonic wall clock
u32 timeMillis();

} // namespace fo
