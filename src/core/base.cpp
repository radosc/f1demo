#include "core/base.h"

#include <windows.h>
#include <cstdarg>
#include <mutex>

namespace fo {

Rng g_rng;

static FILE* g_logFile = nullptr;
static std::mutex g_logMutex;

void logInit(const std::string& path)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) fclose(g_logFile);
    g_logFile = _wfopen(utf8ToWide(path).c_str(), L"w");
}

void logWrite(const char* level, const char* fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    char line[4200];
    snprintf(line, sizeof(line), "[%8.3f] %-5s %s\n", timeSeconds(), level, buf);
    if (g_logFile) {
        fputs(line, g_logFile);
        fflush(g_logFile);
    }
    OutputDebugStringA(line);
}

std::string strLower(std::string s)
{
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string strUpper(std::string s)
{
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

std::string strTrim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::vector<std::string> strSplit(const std::string& s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

bool strStartsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool strEndsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
bool strIEquals(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }

std::string strFormat(const char* fmt, ...)
{
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return {};
    if ((size_t)n < sizeof(buf)) return std::string(buf, n);
    std::string big(n + 1, '\0');
    va_start(ap, fmt);
    vsnprintf(&big[0], big.size(), fmt, ap);
    va_end(ap);
    big.resize(n);
    return big;
}

std::string strReplaceAll(std::string s, const std::string& from, const std::string& to)
{
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string pathNormalize(const std::string& p)
{
    std::string s = strLower(p);
    for (auto& c : s) if (c == '\\') c = '/';
    std::vector<std::string> parts;
    for (auto& part : strSplit(s, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") { if (!parts.empty()) parts.pop_back(); continue; }
        parts.push_back(part);
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += '/';
        out += parts[i];
    }
    return out;
}

std::string pathDir(const std::string& p)
{
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? std::string() : p.substr(0, i);
}

std::string pathFile(const std::string& p)
{
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? p : p.substr(i + 1);
}

std::string pathStem(const std::string& p)
{
    std::string f = pathFile(p);
    size_t i = f.find_last_of('.');
    return i == std::string::npos ? f : f.substr(0, i);
}

std::string pathExt(const std::string& p)
{
    std::string f = pathFile(p);
    size_t i = f.find_last_of('.');
    return i == std::string::npos ? std::string() : f.substr(i);
}

std::string pathJoin(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    char last = a.back();
    if (last == '/' || last == '\\') return a + b;
    return a + "/" + b;
}

std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string wideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

double timeSeconds()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    static LARGE_INTEGER start = [] { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
}

u32 timeMillis() { return (u32)(timeSeconds() * 1000.0); }

} // namespace fo
