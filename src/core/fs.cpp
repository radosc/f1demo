#include "core/fs.h"
#include "core/json.h"

#include <windows.h>

namespace fo {

Vfs g_vfs;

bool fileRead(const std::string& path, Bytes& out)
{
    FILE* f = _wfopen(utf8ToWide(path).c_str(), L"rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    i64 size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    out.resize((size_t)size);
    bool ok = size == 0 || fread(out.data(), 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    return ok;
}

bool fileWrite(const std::string& path, const void* data, size_t size)
{
    std::string dir = pathDir(path);
    if (!dir.empty()) dirCreate(dir);
    FILE* f = _wfopen(utf8ToWide(path).c_str(), L"wb");
    if (!f) {
        LOGE("Cannot write %s", path.c_str());
        return false;
    }
    bool ok = size == 0 || fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

bool fileWrite(const std::string& path, const std::string& text) { return fileWrite(path, text.data(), text.size()); }

bool fileExists(const std::string& path)
{
    DWORD a = GetFileAttributesW(utf8ToWide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::string& path)
{
    DWORD a = GetFileAttributesW(utf8ToWide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirCreate(const std::string& path)
{
    if (path.empty() || dirExists(path)) return true;
    std::string parent = pathDir(path);
    if (!parent.empty() && parent != path && !(parent.size() == 2 && parent[1] == ':')) dirCreate(parent);
    return CreateDirectoryW(utf8ToWide(path).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::vector<std::string> dirList(const std::string& dir, bool wantDirs)
{
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(utf8ToWide(pathJoin(dir, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string name = wideToUtf8(fd.cFileName);
        if (name == "." || name == "..") continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir == wantDirs) out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

std::string exeDir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return pathDir(wideToUtf8(buf));
}

void Vfs::setBaseDir(const std::string& dir)
{
    if (m_layers.empty()) m_layers.push_back(dir);
    else m_layers[0] = dir;
}

void Vfs::pushLayer(const std::string& dir) { m_layers.push_back(dir); }

void Vfs::clearLayers()
{
    if (m_layers.size() > 1) m_layers.resize(1);
}

std::vector<ModInfo> Vfs::discoverMods(const std::string& modsDir)
{
    std::vector<ModInfo> mods;
    if (!dirExists(modsDir)) return mods;
    std::vector<std::string> order;
    std::string listText;
    Bytes listBytes;
    std::string listPath = pathJoin(modsDir, "mods.txt");
    if (fileRead(listPath, listBytes)) {
        listText.assign(listBytes.begin(), listBytes.end());
        for (auto& line : strSplit(listText, '\n')) {
            std::string t = strTrim(line);
            if (t.empty() || t[0] == '#' || t[0] == ';') continue;
            if (t[0] == '-') continue; // "-modname" = disabled
            order.push_back(t);
        }
    } else {
        order = dirList(modsDir, true);
    }
    for (auto& id : order) {
        std::string dir = pathJoin(modsDir, id);
        if (!dirExists(dir)) {
            LOGW("Mod '%s' listed in mods.txt but folder is missing", id.c_str());
            continue;
        }
        ModInfo mi;
        mi.id = id;
        mi.name = id;
        mi.dir = dir;
        Bytes mj;
        if (fileRead(pathJoin(dir, "mod.json"), mj)) {
            Json j = Json::parse(std::string(mj.begin(), mj.end()));
            mi.name = j["name"].str(id);
            mi.description = j["description"].str();
        }
        mods.push_back(mi);
    }
    return mods;
}

std::string Vfs::resolve(const std::string& path) const
{
    std::string norm = pathNormalize(path);
    for (size_t i = m_layers.size(); i-- > 0;) {
        std::string full = pathJoin(m_layers[i], norm);
        if (fileExists(full)) return full;
    }
    return {};
}

std::vector<std::string> Vfs::resolveAll(const std::string& path) const
{
    std::vector<std::string> out;
    std::string norm = pathNormalize(path);
    for (auto& layer : m_layers) {
        std::string full = pathJoin(layer, norm);
        if (fileExists(full)) out.push_back(full);
    }
    return out;
}

bool Vfs::exists(const std::string& path) const { return !resolve(path).empty(); }

bool Vfs::read(const std::string& path, Bytes& out) const
{
    std::string full = resolve(path);
    if (full.empty()) return false;
    return fileRead(full, out);
}

bool Vfs::readText(const std::string& path, std::string& out) const
{
    Bytes b;
    if (!read(path, b)) return false;
    out.assign(b.begin(), b.end());
    return true;
}

std::vector<std::string> Vfs::list(const std::string& dir, const std::string& ext) const
{
    std::set<std::string> names;
    std::string norm = pathNormalize(dir);
    std::string extl = strLower(ext);
    for (auto& layer : m_layers) {
        for (auto& f : dirList(pathJoin(layer, norm), false)) {
            std::string lf = strLower(f);
            if (extl.empty() || strEndsWith(lf, extl)) names.insert(lf);
        }
    }
    return std::vector<std::string>(names.begin(), names.end());
}

} // namespace fo
