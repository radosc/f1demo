// Virtual file system: layered loose-file directories (mods override base data).
#pragma once
#include "core/base.h"

namespace fo {

struct ModInfo {
    std::string id;          // folder name
    std::string name;        // display name (mod.json "name")
    std::string description;
    std::string dir;         // absolute path
};

class Vfs {
public:
    // Base data directory (converted game data) - lowest priority.
    void setBaseDir(const std::string& dir);
    // Adds a directory on top of all current layers (highest priority).
    void pushLayer(const std::string& dir);
    void clearLayers();
    const std::vector<std::string>& layers() const { return m_layers; }

    // Discovers mods inside `modsDir`. Load order: mods.txt if present, otherwise alphabetical.
    std::vector<ModInfo> discoverMods(const std::string& modsDir);

    bool exists(const std::string& path) const;
    bool read(const std::string& path, Bytes& out) const;
    bool readText(const std::string& path, std::string& out) const;
    // Absolute path of the highest priority file providing `path`, or empty.
    std::string resolve(const std::string& path) const;
    // Every layer's copy of `path`, lowest priority (base data) first.
    std::vector<std::string> resolveAll(const std::string& path) const;
    // Merged listing of files directly inside `dir` with optional extension filter (".png").
    std::vector<std::string> list(const std::string& dir, const std::string& ext = "") const;

private:
    std::vector<std::string> m_layers; // index 0 = lowest priority
};

extern Vfs g_vfs;

// Plain OS file helpers (absolute or relative to the working directory).
bool fileRead(const std::string& path, Bytes& out);
bool fileWrite(const std::string& path, const void* data, size_t size);
bool fileWrite(const std::string& path, const std::string& text);
bool fileExists(const std::string& path);
bool dirExists(const std::string& path);
bool dirCreate(const std::string& path); // recursive
std::vector<std::string> dirList(const std::string& dir, bool wantDirs);
std::string exeDir();

} // namespace fo
