// Object prototypes (items, critters, scenery, walls, tiles, misc) loaded from data/proto/<type>/<index>.json
#pragma once
#include "game/defs.h"

namespace fo {

struct ArmorData {
    int ac = 0;
    int dr[DMG_TYPE_COUNT] = {};
    int dt[DMG_TYPE_COUNT] = {};
    int perk = -1;
    int maleFid = -1, femaleFid = -1;
};
struct ContainerData { int maxSize = 0; u32 openFlags = 0; };
struct DrugData {
    int stat[3] = { -1, -1, -1 };
    int amount[3] = {};
    int duration1 = 0;
    int amount1[3] = {};
    int duration2 = 0;
    int amount2[3] = {};
    int addictionChance = 0, withdrawalEffect = 0, withdrawalOnset = 0;
};
struct WeaponData {
    int animCode = 0;
    int minDamage = 0, maxDamage = 0;
    int damageType = 0;
    int range1 = 0, range2 = 0;
    int projectilePid = -1;
    int minStrength = 0;
    int ap1 = 0, ap2 = 0;
    int criticalFail = 0;
    int perk = -1;
    int burstRounds = 0;
    int caliber = 0;
    int ammoPid = -1;
    int maxAmmo = 0;
    int soundCode = 0; // character used to build weapon sfx names
};
struct AmmoData { int caliber = 0, quantity = 0, acMod = 0, drMod = 0, dmgMult = 1, dmgDiv = 1; };
struct MiscItemData { int powerPid = -1, powerType = 0, charges = 0; };
struct KeyData { int keyCode = -1; };

struct Proto {
    int pid = -1;
    int messageId = 0;
    int fid = -1;
    int lightDistance = 0, lightIntensity = 0;
    u32 flags = 0, extFlags = 0;
    int sid = -1; // (scriptType << 24) | scriptIndex, or -1

    // items
    int itemType = 0, material = 0, size = 0, weight = 0, cost = 0, invFid = -1, soundId = 0;
    ArmorData armor;
    ContainerData container;
    DrugData drug;
    WeaponData weapon;
    AmmoData ammo;
    MiscItemData misc;
    KeyData key;

    // critters
    int headFid = -1, aiPacket = 0, team = 0;
    u32 critterFlags = 0;
    int baseStats[STAT_SAVEABLE_COUNT] = {};
    int bonusStats[STAT_SAVEABLE_COUNT] = {};
    int skills[SKILL_COUNT] = {};
    int bodyType = 0, experience = 0, killType = 0;

    // scenery: door {openFlags, keyCode}; stairs {destMap, destTile}; elevator {type, level}; ladders {destTile}
    int sceneryType = SCEN_GENERIC;
    int sceneryData[2] = { -1, -1 };

    int type() const { return pidType(pid); }
    int primaryAttack() const { return (int)(extFlags & 0xF); }
    int secondaryAttack() const { return (int)((extFlags >> 4) & 0xF); }

    Json toJson(const std::string& nameComment) const;
    bool fromJson(const Json& j);
};

class ProtoDb {
public:
    void clear() { m_cache.clear(); }
    // nullptr if missing
    const Proto* get(int pid);
    Proto* getMutable(int pid) { return const_cast<Proto*>(get(pid)); }
    // Registers an in-memory proto (e.g. the player's), replacing any cached one.
    Proto* put(const Proto& p);
    static std::string pathFor(int pid);
    // Proto names / descriptions from text/english/game/pro_*.msg
    std::string name(int pid);
    std::string description(int pid);

private:
    std::unordered_map<int, std::unique_ptr<Proto>> m_cache;
    std::set<int> m_missing;
};

extern ProtoDb g_protos;

} // namespace fo
