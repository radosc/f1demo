// FID (art id) resolution through the art/<type>/<type>.lst lists.
#pragma once
#include "game/defs.h"
#include "gfx/sprite.h"

namespace fo {

class ArtDb {
public:
    void load();
    void reset() { m_loaded = false; }
    // "art/critters/hmjmpsaa" or empty if the fid is invalid.
    std::string pathFor(int fid);
    // Resolves (with fallbacks for missing critter animations) and returns the sprite.
    Sprite* sprite(int fid);
    bool exists(int fid) { return sprite(fid) != nullptr; }
    int count(int type);
    std::string name(int type, int index); // list entry (without extension)
    int indexOf(int type, const std::string& name);
    // FID for an interface/inventory/etc. art by file name ("iface" -> fid)
    int fidByName(int type, const std::string& name);

private:
    struct List {
        std::vector<std::string> names;
        std::vector<int> alias; // critters only
        std::unordered_map<std::string, int> byName;
    };
    void ensure();
    bool m_loaded = false;
    List m_lists[OBJ_TYPE_COUNT];
    std::unordered_map<int, std::string> m_pathCache;
};

extern ArtDb g_art;

// Critter animation code letters (weapon code, anim code); returns false for invalid combos.
bool artCritterCode(int anim, int weapon, char& c1, char& c2);

} // namespace fo
