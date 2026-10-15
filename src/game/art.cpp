#include "game/art.h"
#include "core/fs.h"

namespace fo {

ArtDb g_art;

bool artCritterCode(int anim, int weapon, char& c1, char& c2)
{
    if (weapon < 0 || weapon >= WANIM_COUNT) return false;
    if (anim >= ANIM_TAKE_OUT && anim <= ANIM_FIRE_CONTINUOUS) {
        c2 = (char)('c' + (anim - ANIM_TAKE_OUT));
        if (weapon == WANIM_NONE) return false;
        c1 = (char)('d' + (weapon - 1));
        return true;
    }
    if (anim == ANIM_PRONE_TO_STANDING) { c2 = 'h'; c1 = 'c'; return true; }
    if (anim == ANIM_BACK_TO_STANDING) { c2 = 'j'; c1 = 'c'; return true; }
    if (anim == ANIM_CALLED_SHOT_PIC) { c2 = 'a'; c1 = 'n'; return true; }
    if (anim >= ANIM_FIRST_SF_DEATH) { c2 = (char)('a' + (anim - ANIM_FIRST_SF_DEATH)); c1 = 'r'; return true; }
    if (anim >= ANIM_FIRST_DEATH) { c2 = (char)('a' + (anim - ANIM_FIRST_DEATH)); c1 = 'b'; return true; }
    if (anim == ANIM_THROW_ANIM) {
        if (weapon == WANIM_KNIFE) { c1 = 'd'; c2 = 'm'; }
        else if (weapon == WANIM_SPEAR) { c1 = 'g'; c2 = 'm'; }
        else { c1 = 'a'; c2 = 's'; }
        return true;
    }
    if (anim == ANIM_DODGE) {
        if (weapon <= 0) { c1 = 'a'; c2 = 'n'; }
        else { c1 = (char)('d' + (weapon - 1)); c2 = 'e'; }
        return true;
    }
    c2 = (char)('a' + anim);
    if (anim <= ANIM_WALK && weapon > 0) c1 = (char)('d' + (weapon - 1));
    else c1 = 'a';
    return true;
}

void ArtDb::ensure()
{
    if (!m_loaded) load();
}

void ArtDb::load()
{
    m_loaded = true;
    m_pathCache.clear();
    for (int t = 0; t < OBJ_TYPE_COUNT; t++) {
        List& l = m_lists[t];
        l.names.clear();
        l.alias.clear();
        l.byName.clear();
        std::string dir = objTypeDir(t);
        std::string text;
        if (!g_vfs.readText("art/" + dir + "/" + dir + ".lst", text)) continue;
        for (auto& raw : strSplit(text, '\n')) {
            std::string line = raw;
            size_t semi = line.find(';');
            if (semi != std::string::npos) line = line.substr(0, semi);
            line = strLower(strTrim(line));
            int alias = -1;
            if (t == OBJ_CRITTER) {
                auto parts = strSplit(line, ',');
                line = strTrim(parts[0]);
                if (parts.size() > 1) alias = atoi(parts[1].c_str());
            }
            // strip extension
            size_t dot = line.find('.');
            if (dot != std::string::npos) line = line.substr(0, dot);
            if (!line.empty() && !l.byName.count(line)) l.byName[line] = (int)l.names.size();
            l.names.push_back(line);
            l.alias.push_back(alias);
        }
    }
}

int ArtDb::count(int type)
{
    ensure();
    return (type >= 0 && type < OBJ_TYPE_COUNT) ? (int)m_lists[type].names.size() : 0;
}

std::string ArtDb::name(int type, int index)
{
    ensure();
    if (type < 0 || type >= OBJ_TYPE_COUNT) return {};
    const List& l = m_lists[type];
    return (index >= 0 && index < (int)l.names.size()) ? l.names[index] : std::string();
}

int ArtDb::indexOf(int type, const std::string& n)
{
    ensure();
    if (type < 0 || type >= OBJ_TYPE_COUNT) return -1;
    std::string key = strLower(n);
    size_t dot = key.find('.');
    if (dot != std::string::npos) key = key.substr(0, dot);
    auto it = m_lists[type].byName.find(key);
    return it == m_lists[type].byName.end() ? -1 : it->second;
}

int ArtDb::fidByName(int type, const std::string& n)
{
    int idx = indexOf(type, n);
    return idx < 0 ? -1 : makeFid(type, idx);
}

std::string ArtDb::pathFor(int fid)
{
    ensure();
    if (fid < 0) return {};
    auto it = m_pathCache.find(fid);
    if (it != m_pathCache.end()) return it->second;
    int type = fidType(fid);
    int index = fidIndex(fid);
    std::string result;
    if (type >= 0 && type < OBJ_TYPE_COUNT && index < (int)m_lists[type].names.size()) {
        const std::string& base = m_lists[type].names[index];
        std::string dir = objTypeDir(type);
        if (!base.empty()) {
            if (type == OBJ_CRITTER) {
                char c1, c2;
                if (artCritterCode(fidAnim(fid), fidWeapon(fid), c1, c2)) result = "art/" + dir + "/" + base + c1 + c2;
            } else {
                result = "art/" + dir + "/" + base;
            }
        }
    }
    m_pathCache[fid] = result;
    return result;
}

Sprite* ArtDb::sprite(int fid)
{
    if (fid < 0) return nullptr;
    int clean = fid & 0x0FFFFFFF; // rotation bits select directional files, which the converter merged
    std::string p = pathFor(clean);
    if (p.empty()) return nullptr;
    Sprite* s = g_sprites.get(p);
    if (s) return s;
    if (fidType(clean) == OBJ_CRITTER) {
        int anim = fidAnim(clean);
        // special deaths reuse the alias critter's art (critters.lst "name,alias")
        const List& l = m_lists[OBJ_CRITTER];
        int idx = fidIndex(clean);
        if (idx < (int)l.alias.size() && l.alias[idx] >= 0 && l.alias[idx] != idx) {
            Sprite* a = g_sprites.get(pathFor(makeFid(OBJ_CRITTER, l.alias[idx], anim, fidWeapon(clean))));
            if (a) return a;
        }
    }
    return nullptr;
}

} // namespace fo
